#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include <threads/thread.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"

/** A directory. */
struct dir 
  {
    struct inode *inode;                /**< Backing store. */
    off_t pos;                          /**< Current position. */
  };

/** A single directory entry. */
struct dir_entry 
  {
    // TODO inode_sector干啥的: 这个目录项对应文件的inode
    // on-disk inode sector number, 一个struct inode有一个on-disk inode sector number对应的data
    block_sector_t inode_sector;        /**< Sector number of header. */
    char name[NAME_MAX + 1];            /**< Null terminated file name. */
    bool in_use;                        /**< In use or free? */
    bool is_dir;
  };

/** Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (block_sector_t sector, size_t entry_cnt, block_sector_t parent_dir_sector)
{
  // inode_create中会把内容清零
  bool success = inode_create (sector, (entry_cnt + 2) * sizeof (struct dir_entry), DIRECTORY);
  if (success) {
    struct inode *inode = inode_open (sector);
    ASSERT(inode != NULL);

    struct dir_entry entry;
    entry.inode_sector = sector;
    strlcpy(entry.name, ".", 2);
    entry.in_use = true;
    entry.is_dir = true;
    if (inode_write_at (inode, &entry, sizeof entry, 0) != sizeof (entry)) {
      success = false;
    }

    entry.inode_sector = parent_dir_sector;
    strlcpy(entry.name, "..", 3);
    entry.in_use = true;
    entry.is_dir = true;
    if (inode_write_at (inode, &entry, sizeof entry, sizeof (struct dir_entry)) != sizeof (entry)) {
      success = false;
    }

    inode_close(inode);
  }
  return success;
}

// 为inode创建一个dir来表示目录当前状态(pos)
/** Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) 
{
  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && dir != NULL)
    {
      dir->inode = inode;
      dir->pos = 2 * sizeof (struct dir_entry);
      return dir;
    }
  else
    {
      inode_close (inode);
      free (dir);
      return NULL; 
    }
}

static int
next_slash (const char *path) {
  size_t len = strlen(path);
  int res = 0;
  while (res < len && path[res] != '/') {
    res++;
  }
  return res == len ? -1 : res;
}

struct dir *
dir_open_path (const char *path)
{
  // TODO 这里直接使用的root,后续要改
  // TODO 改成open当前进程的pwd或者root(如果name里第一个是/)
  // TODO 现在先不考虑结尾可以是/的情况
  // /a/b/c, 只会打开/a/b, 从pwd打开到最后的/
  struct dir *dir;
  int slash_index = next_slash(path);
  if (slash_index == 0) {
    dir = dir_open_root ();
    path++;
  } else {
    dir = dir_open_pwd();
  }
  while (true) {
    char entry_name[15];
    slash_index = next_slash(path);
    if (slash_index == -1) {
      break;
    }
    ASSERT(slash_index < 15);
    if (slash_index == 0) {
      // TODO 处理 //
      PANIC("filesys_create");
    }
    strlcpy(entry_name, path, slash_index + 1);
    // printf("entry name: %s\n", entry_name);
    // TODO ., ..
    ASSERT(path[slash_index] == '/');
    path += slash_index + 1;
    entry_name[slash_index] = '\0';
    struct inode *inode;
    dir_lookup(dir, entry_name, &inode, true);
    if (inode == NULL) {
      // TODO dir_close
      dir_close(dir);
      return NULL;
    }
    dir_close(dir);
    dir = dir_open(inode);
  }
  return dir;
}

struct dir *
dir_open_pwd (void)
{
  return dir_open (inode_open (thread_current()->current_dir_sector));
}

/** Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root (void)
{
  return dir_open (inode_open (ROOT_DIR_SECTOR));
}

/** Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) 
{
  return dir_open (inode_reopen (dir->inode));
}

/** Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
  if (dir != NULL)
    {
      inode_close (dir->inode);
      free (dir);
    }
}

/** Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) 
{
  return dir->inode;
}

// 在dir对应的inode文件中查找name对应的dir_entry(对比从inode中读取到的所有dir_entry)
/** Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp) 
{
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (e.in_use && !strcmp (name, e.name)) 
      {
        if (ep != NULL)
          *ep = e;
        if (ofsp != NULL)
          *ofsp = ofs;
        return true;
      }
  return false;
}

// 根据文件名name在dir中找对应的inode
/** Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
            struct inode **inode, bool need_dir)
{
  struct dir_entry e;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  if (lookup (dir, name, &e, NULL)) {
    if (need_dir && e.is_dir == false) {
      *inode = NULL;
    } else {
      *inode = inode_open (e.inode_sector);
    }
  }
  else
    *inode = NULL;

  return *inode != NULL;
}

// 将文件名为name的文件(inode在inode_sector中)添加到dir目录中,
// 就是向dir对应的inode文件中加一个dir_entry
/** Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add (struct dir *dir, const char *name, block_sector_t inode_sector, bool is_dir)
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Check NAME for validity. */
  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;

  /* Check that NAME is not in use. */
  if (lookup (dir, name, NULL, NULL))
    goto done;

  // 如果没有空闲位置, 则将设置为当前的文件末尾, 自动扩容
  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.
     
     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  e.is_dir = is_dir;
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

 done:
  return success;
}

/** Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) 
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Find directory entry. */
  if (!lookup (dir, name, &e, &ofs))
    goto done;

  /* Open inode. */
  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;

  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e) 
    goto done;

  /* Remove inode. */
  inode_remove (inode);
  success = true;

 done:
  inode_close (inode);
  return success;
}

/** Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  struct dir_entry e;

  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) 
    {
      dir->pos += sizeof e;
      if (e.in_use)
        {
          strlcpy (name, e.name, NAME_MAX + 1);
          return true;
        } 
    }
  return false;
}
